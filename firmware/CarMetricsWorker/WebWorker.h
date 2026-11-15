// CarMetrics worker: its own API routes on top of the shared ones (CarMetricsCore/Web).
#pragma once
namespace WebWorker {
void begin();   // registers the worker's routes and hooks, then starts the web server
}
