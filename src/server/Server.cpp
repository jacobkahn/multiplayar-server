#include "include/server/Server.hpp"
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include "include/cv/SIFT.hpp"
#include "include/cv/SIFTWriter.hpp"
#include "include/deps/crow.h"

// Relevant headers for request parsing
const std::string kUserIDHeaderValueHeader = "x-user-id";
const std::string kBestCandidateLocationsHeader = "x-points";
const std::string kObjectUpdateIDHeader = "x-object-id";
const std::string kXLocationHeader = "x-xcord";
const std::string kYLocationHeader = "x-ycord";
const std::string kZLocationHeader = "x-zcord";
const std::string kRotationHeader = "x-rotation";
// JSON data field names
const std::string kImageQueryIDFieldName = "id";
const std::string kImageQueryID1FieldName = "id1";
const std::string kImageQueryID2FieldName = "id2";
const std::string kImageQueryStageFieldName = "stage";

Server::Server(bool debugMode, bool writeImageMode)
    : debugMode_(debugMode), writeImageMode_(writeImageMode) {}

void Server::run(uint32_t port) {
  // Start main server thread
  app.port(port).multithreaded().run();
}

crow::json::wvalue Server::mapToCrowWValue(
    std::unordered_map<std::string, std::string> map) {
  crow::json::wvalue val;
  for (auto& kv : map) {
    val[kv.first] = kv.second;
  }
  return val;
}

void Server::setup() {
  CROW_ROUTE(app, "/").methods("GET"_method)([](const crow::request& req) {
    return crow::response("Multiplayar Core");
  });

  /**
   * Processes an orientation image from a client. Runs SIFT on the incoming
   * image in conjunction with an image from another client, and computes a
   * homography and perspective transform while finding the local candidate
   * coordinates from ARKit that are a best fit (as for these points, there is
   * better information about their 2D-3D mapping).
   *
   * @header user id: the id of the user sending the image.
   * @header candidate points: a semicolon-delimited list of comma-delimited 2D
   *     points that are good candidates as per ARKit
   * @response: four points computed via a homography and perspective transform
   *     which are candidate points closest to the points computed by ARKit.
   */
  CROW_ROUTE(app, "/image")
      .methods("POST"_method)([&](const crow::request& req) {
        // Get user id and image from the request
        std::string id = req.headers.find(kUserIDHeaderValueHeader)->second;
        // Get candidate points that ARKit detects
        std::string rawCandidatePoints;
        auto candidatePointHeader =
            req.headers.find(kBestCandidateLocationsHeader);
        if (candidatePointHeader != req.headers.end()) {
          rawCandidatePoints = candidatePointHeader->second;
        }
        /***** Process Candidate Points *****/
        // Parse request string into list of L2-calcuable points
        // Final collection of candidate points
        std::vector<cv::Point2f> candidatePoints;
        if (!rawCandidatePoints.empty()) {
          // Point strings
          std::vector<std::string> pointStrings;
          // Delimited by semicolon, i.e. 1,2;4,5;3,4
          boost::split(pointStrings, rawCandidatePoints, boost::is_any_of(";"));
          // Split each point
          for (auto pointString : pointStrings) {
            // Individual point string
            std::vector<std::string> aPointString;
            boost::split(aPointString, pointString, boost::is_any_of(","));
            candidatePoints.push_back(cv::Point2f(
                std::stof(aPointString[0]), std::stof(aPointString[1])));
          }
        }

        std::string image = req.body;
        // If we're writing image data
        // Add a user to the environment or update an existing user
        auto siftOutPoints =
            environment_.updateClient(id, std::move(image), candidatePoints);

        // Write raw image data in a separate thread
        // auto writeThread = std::thread([&]() {
        std::cout << "Write original image\n";
        auto writer = std::make_shared<SIFTWriter>();
        std::ofstream outStream1(writer->computeSingleFilenameForID(id, 1));
        outStream1 << image;
        outStream1.close();
        std::cout << "Write keypoints and ar points\n";
        writer->createImageWithKeypointsAndARPoints(
            id,
            environment_.getClientByID(id)->getKeypoints(),
            candidatePoints);
        // });
        // writeThread.detach();

        // Format points for transport - json
        std::vector<crow::json::wvalue> pointList;
        for (auto& aSiftPoint : siftOutPoints) {
          pointList.push_back(mapToCrowWValue(aSiftPoint));
        }
        // Format JSON response
        crow::json::wvalue response;
        response["points"] = std::move(pointList);
        return crow::response(response);
      });

  /**
   * Endpoint for object manipulation in the environment
   *
   * @header object id: the id of the object being manipulated. Empty if a new
   * object is being created.
   * @header location: the location of the new object or of the new position of
   * the object being updated.
   * @response: the id of the new or modified object.
   */
  CROW_ROUTE(app, "/object")
      .methods("POST"_method)([&](const crow::request& req) {
        // Get the updated location of the object in question
        std::string xLoc = req.headers.find(kXLocationHeader)->second;
        std::string yLoc = req.headers.find(kYLocationHeader)->second;
        std::string zLoc = req.headers.find(kZLocationHeader)->second;
        std::string rawRotation = req.headers.find(kRotationHeader)->second;

        if (debugMode_) {
          std::cout << "Object location update at (" << xLoc << ", " << yLoc
                    << ", " << zLoc << ")\n";
        }

        Location objectLocation =
            cv::Point3d(std::stof(xLoc), std::stof(yLoc), std::stof(zLoc));
        auto rotation = std::stof(rawRotation);
        // See if we're working with an existing object ID. If so, go forth
        // and update it: otherwise, create a new object. Always respond
        // with the ID
        std::string objectID;
        auto objectIDHeader = req.headers.find(kObjectUpdateIDHeader);
        if (objectIDHeader != req.headers.end()) {
          // Modify existing object
          objectID = objectIDHeader->second;
        } else {
          // Create a new object with a sequentially-generated ID
          objectID = environment_.addObject();
        }
        environment_.updateObject(objectID, objectLocation, rotation);
        // Format JSON response
        return crow::response(objectID);
      });

  /**
   * Returns a description of the augmented reality environment. This includes:
   * - All AR objects as created by clients.
   *
   * @response: json payload with data
   *
   * Serialization format (json):
   * {
   *    objects: [
   *      {
   *        id: [id],
   *        x: [double],
   *        y: [double],
   *        z: [double]
   *      }
   *    ],
   *    ...
   * }
   */
  CROW_ROUTE(app, "/sync").methods("GET"_method)([&](const crow::request& req) {
    // Get environment data
    auto objectData = environment_.getObjectRepresentation();

    // Serialize
    std::vector<crow::json::wvalue> objectList;
    for (auto& object : objectData) {
      objectList.push_back(mapToCrowWValue(object));
    }
    // Format JSON response
    crow::json::wvalue response;
    response["objects"] = std::move(objectList);
    // Users
    std::vector<crow::json::wvalue> userList;
    response["users"] = std::move(userList);
    // TODO: send them a user field????
    return crow::response(response);
  });

  /**
   * A point that has been selected by the client as an anchor
   *
   * @param: entity ID - user id
   * @param: A 2D world coordinate
   *
   * @response: 200 OK
   */
  CROW_ROUTE(app, "/anchor")
      .methods("POST"_method)([&](const crow::request& req) {
        // Get user id  from the request
        std::string id = req.headers.find(kUserIDHeaderValueHeader)->second;
        std::string pointX = req.headers.find(kXLocationHeader)->second;
        std::string pointY = req.headers.find(kYLocationHeader)->second;
        auto point = cv::Point2f(std::stod(pointX), std::stod(pointY));
        // Update anchor points for this client. Stores the point, computes a
        // homography and applies relevant anchor points to all clients who
        // don't have anchor points, etc. Any clients who are polling for anchor
        // points will receive responses once this action completes
        environment_.update2DAnchorForClient(id, point);
        // Empty response (endpoing returns nothing on success)
        return crow::response("");
      });

  /**
   * Allows a client to poll to see if they have valid anchor points. If they
   * were the first client to calibrate or were not a special client who
   * receives AR points in their response, then they will recieve a point as
   * soon as another client has calibrated and chosen their anchor point, during
   * which time the appropriate SIFT point local to the polling point will be
   * computed with the original homography between the clients (which computed
   * the first client's SIFT points, and the related best-candidate AR points).
   *
   * @param: entity ID - the id of the user
   * @response: if there is no anchor available for this user, an empty response
   * is retured. Otherwise, an object containing the 2D anchor point is returned
   * as follows:
   * {
   *   x: [string],
   *   y: [string]
   * }
   */
  CROW_ROUTE(app, "/pointpoll")
      .methods("POST"_method)([&](const crow::request& req) {
        // Check and see if there is a homography and a selected point,
        // transform and respond with point if true
        std::string id = req.headers.find(kUserIDHeaderValueHeader)->second;
        auto client = environment_.getClientByID(id);
        if (client->hasInitializedAnchor()) {
          auto stringyPoint = PointRepresentationUtils::cvPoint2fToStringyPoint(
              client->get2DAnchorPoint());
          crow::json::wvalue response;
          response["x"] =
              stringyPoint[PointRepresentationUtils::kStringyPointXFieldName];
          response["y"] =
              stringyPoint[PointRepresentationUtils::kStringyPointYFieldName];
          return crow::response(response);
        } else {
          // If no anchor has been initialized, send back the empty string
          return crow::response("");
        }
      });

  /**
   * Clears the environment of all AR users and AR objects. Equivalent to a full
   * reset of the server
   */
  CROW_ROUTE(app, "/clear")
      .methods("GET"_method)([&](const crow::request& req) {
        // Clear the environment
        environment_.clear();
        return crow::response("");
      });

  /**
   * Returns all ID for clients currently associated with the environment
   */
  CROW_ROUTE(app, "/ids").methods("GET"_method)([&](const crow::request& req) {
    std::vector<crow::json::wvalue> ids;
    for (auto& client : environment_.getClientList()) {
      crow::json::wvalue idItem;
      idItem["id"] = client->getID();
      ids.push_back(std::move(idItem));
    }
    crow::json::wvalue response;
    response["ids"] = std::move(ids);
    return crow::response(response);
  });

  /**
   * Gets the image
   */
  CROW_ROUTE(app, "/imagequery")
      .methods("GET"_method)([&](const crow::request& req) {
        auto json = crow::json::load(req.body);
        if (!json) {
          return crow::response(400);
        }

        std::string responseBuffer;
        // Image stage
        auto stage = json[kImageQueryStageFieldName].i();
        if (stage < 1 || stage > 4) {
          return crow::response(400);
        }
        // Create a writer to get data
        auto writer = std::make_shared<SIFTWriter>();
        // Process
        if (json.has(kImageQueryIDFieldName)) {
          // This is a single query
          auto id = json[kImageQueryIDFieldName].s();

          if (debugMode_) {
            std::cout << "Single image query for id " << id << " at stage "
                      << stage << "\n";
          }

          responseBuffer = writer->getSingleImageDataForID(id, stage);
        } else {
          // Compound query
          auto id1 = json[kImageQueryID1FieldName].s();
          auto id2 = json[kImageQueryID2FieldName].s();

          if (debugMode_) {
            std::cout << "Duplex image query for id1: " << id1
                      << ", id2: " << id2 << " at stage " << stage << "\n";
          }

          // Get images
          responseBuffer = writer->getCompoundImageDataForIDs(id1, id2, stage);
        }

        return crow::response(responseBuffer);
      });
}
