#include "include/environment/Environment.hpp"
#include <boost/functional/hash.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "include/environment/Client.hpp"
#include "include/environment/Object.hpp"

PointList Environment::updateClient(
    EntityID id,
    std::string image,
    std::vector<cv::Point2f> candidatePoints) {
  std::shared_ptr<Client> client;
  if (clientExists(id)) {
    client = clientsByID.find(id)->second;
    client->processImage(std::move(image));
  } else {
    client = std::make_shared<Client>(id);
    client->processImage(std::move(image));
    clients.push_back(client);
    clientsByID.insert(std::make_pair(id, client));
  }

  // Attempt to compute homography with another client
  // Make sure we're not the only client
  if (clients.size() > 1) {
    // Get not you
    for (auto& otherClient : clients) {
      if (otherClient->getID() != client->getID()) {
        // We found another client that isn't us. Compute homography
        auto siftClient = std::make_unique<SIFTClient>();
        auto result = siftClient->computeHomographyTransformationFromClients(
            client, otherClient);
        /***** Choose the closest candidate points to our output points *****/
        // New collection of AR-kit candidate points
        PointList bestCandidatePoints;
        // We perform a brute force algorithm over which the runtime is 4
        // times the total number of ARKit-detected points. This is a minor
        // compuation: we're simply computing an L2 metric over the point
        // set
        std::cout << "Matched candidate points:\n";
        for (auto& point : result.first) {
          // Look through candidate points, track the lowest current point
          double minDistance = 0.0;
          cv::Point2f bestPoint;
          for (auto& candidatePoint : candidatePoints) {
            auto bestDistance = cv::norm(
                cv::Mat(
                    cv::Point2f(std::stod(point["x"]), std::stod(point["y"]))),
                cv::Mat(candidatePoint));
            // Check if this is the best candidate point
            if (minDistance < bestDistance) {
              minDistance = bestDistance;
              bestPoint = candidatePoint;
            }
          }
          // Add best candidate point to set
          bestCandidatePoints.push_back({{"x", std::to_string(bestPoint.x)},
                                         {"y", std::to_string(bestPoint.y)}});
          std::cout << "p: (" << bestPoint.x << ", " << bestPoint.y << ")\n";
        }
        // result.first;
        // We only care about the first point list - that's the calling user's
        return bestCandidatePoints;
      }
    }
  }
  return {};
}

bool Environment::clientExists(const EntityID& id) {
  return clientsByID.find(id) != clientsByID.end();
}

EntityID Environment::addObject() {
  // TODO: make this and all entities into actual UUID generation
  // Create new object with the next ID
  auto object = std::make_shared<Object>(std::to_string(objects.size() + 1));
  // Add to collections
  objects.push_back(object);
  objectsByID.insert(std::make_pair(object->getID(), object));
  return object->getID();
}

void Environment::updateObject(EntityID id, Location location) {
  auto iter = objectsByID.find(id);
  if (iter != objectsByID.end()) {
    iter->second->updateLocation(location);
  } else {
    // Invalid object ID: simply return and do nothing
  }
}

ObjectData Environment::getObjectRepresentation() {
  ObjectData data;
  for (auto& object : objects) {
    auto location = object->getLocation();
    data.push_back({{"id", object->getID()},
                    {"x", std::to_string(location.x)},
                    {"y", std::to_string(location.y)},
                    {"z", std::to_string(location.z)}});
  }
  return data;
}