// CarMetrics main board: its own API routes on top of the shared ones (CarMetricsCore/Web).
#pragma once
namespace WebMain {
void begin();   // registers the main board's routes and hooks, then starts the web server
}
