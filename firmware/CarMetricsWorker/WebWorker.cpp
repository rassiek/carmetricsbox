// CarMetrics worker - API routes only a worker has: its link to the main board.
// Everything else (its sensors and actuators, settings, pins, network, firmware)
// is the shared API, so the same web app runs here on its own.
#include "WebWorker.h"
#include "Web.h"
#include "Node.h"

using namespace Web;

namespace {
void hNode(Ctx& c) { JsonDocument d; Node::toJson(d.to<JsonObject>()); sendJson(c.req, 200, d); }
void hUnpair(Ctx& c) { sendResult(c.req, Node::unpair(), "Forgot the main board: restarting"); }

const Route kWorkerRoutes[] = {
  {HTTP_GET, "node", SCOPE_READ, hNode, nullptr},
  {HTTP_POST, "node/unpair", SCOPE_ADMIN, hUnpair, nullptr},
};

void info(JsonObject d) {
  d["role"] = "worker";
  JsonArray cap = d["capabilities"];
  cap.add("standalone");
  cap.add("customBoard");
#ifdef CM_WORKER_PLUS
  cap.add("history");
#endif
}
}  // namespace

void WebWorker::begin() {
  Web::addRoutes(kWorkerRoutes, sizeof(kWorkerRoutes) / sizeof(kWorkerRoutes[0]));
  Web::setInfo(info);
  Web::begin();
}
