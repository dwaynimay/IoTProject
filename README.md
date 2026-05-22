
```
IoTProject
├─ .agents
│  ├─ rules
│  │  └─ graphify.md
│  ├─ skills
│  │  ├─ cavecrew
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-commit
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-compress
│  │  │  ├─ README.md
│  │  │  ├─ scripts
│  │  │  │  ├─ benchmark.py
│  │  │  │  ├─ cli.py
│  │  │  │  ├─ compress.py
│  │  │  │  ├─ detect.py
│  │  │  │  ├─ validate.py
│  │  │  │  ├─ __init__.py
│  │  │  │  └─ __main__.py
│  │  │  ├─ SECURITY.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-help
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-review
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  └─ caveman-stats
│  │     ├─ README.md
│  │     └─ SKILL.md
│  └─ workflows
│     └─ graphify.md
├─ archive
│  ├─ firmware
│  │  ├─ CS_Sensor_gaussian_lasso.cpp
│  │  └─ CS_Sensor_gaussian_lasso.h
│  ├─ README.md
│  └─ server
│     ├─ config.py
│     ├─ cs_utils.py
│     └─ test_single_signal.py
├─ docs
│  ├─ breadboard_v01.png
│  ├─ breadboard_v02.png
│  ├─ mesh_architecture.md
│  ├─ prototype_v01.jpeg
│  ├─ prototype_v02_bottom.jpeg
│  ├─ prototype_v02_top.jpeg
│  ├─ schematic_v01.png
│  └─ schematic_v02.png
├─ firmware
│  ├─ include
│  │  ├─ config
│  │  │  ├─ features.h
│  │  │  ├─ hardware.h
│  │  │  ├─ mesh_settings.h
│  │  │  └─ tuning.h
│  │  ├─ Config.h
│  │  └─ utils
│  │     └─ Logger.h
│  ├─ lib
│  │  ├─ CS_Model_Gaussian
│  │  │  ├─ CS_Sensor.cpp
│  │  │  └─ CS_Sensor.h
│  │  ├─ CS_Model_Lasso
│  │  │  ├─ CS_Sensor.cpp
│  │  │  └─ CS_Sensor.h
│  │  ├─ EspNowMesh
│  │  │  ├─ EspNowMesh.cpp
│  │  │  ├─ EspNowMesh.h
│  │  │  ├─ MeshPackets.h
│  │  │  ├─ MeshRouting.cpp
│  │  │  └─ MeshRouting.h
│  │  ├─ HealthSensors
│  │  │  ├─ Sensor_MPU.cpp
│  │  │  ├─ Sensor_MPU.h
│  │  │  ├─ Sensor_PPG.cpp
│  │  │  └─ Sensor_PPG.h
│  │  ├─ Network_Mqtt
│  │  │  ├─ Network_Mqtt.cpp
│  │  │  └─ Network_Mqtt.h
│  │  ├─ Routing
│  │  │  ├─ DynamicRouter.cpp
│  │  │  └─ DynamicRouter.h
│  │  └─ Watchdog
│  │     ├─ Watchdog.cpp
│  │     └─ Watchdog.h
│  ├─ platformio.ini
│  ├─ src
│  │  ├─ main.cpp
│  │  ├─ task_cs_sender.cpp
│  │  └─ task_mesh_handler.cpp
│  └─ test
│     ├─ test_cs_encoder.cpp
│     └─ test_mesh_routing.cpp
├─ graphify-out
│  ├─ .graphify_labels.json
│  ├─ .graphify_python
│  ├─ .graphify_root
│  ├─ cost.json
│  ├─ graph.html
│  ├─ graph.json
│  ├─ GRAPH_REPORT.md
│  └─ manifest.json
├─ README.md
├─ server
│  ├─ .env.example
│  ├─ apps
│  │  ├─ dashboard_server.py
│  │  ├─ main_app.py
│  │  ├─ reconstruct
│  │  │  ├─ listener.py
│  │  │  ├─ node_state.py
│  │  │  ├─ processor.py
│  │  │  ├─ __init__.py
│  │  │  └─ __main__.py
│  │  └─ __init__.py
│  ├─ core
│  │  ├─ config.example.py
│  │  ├─ config.py
│  │  ├─ cs_gaussian.py
│  │  ├─ cs_lasso.py
│  │  ├─ cs_router.py
│  │  ├─ inference.py
│  │  ├─ logger.py
│  │  ├─ quality.py
│  │  ├─ storage.py
│  │  ├─ validator.py
│  │  └─ __init__.py
│  ├─ pyproject.toml
│  ├─ requirements-dev.txt
│  ├─ requirements.txt
│  ├─ static
│  │  ├─ css
│  │  │  └─ style.css
│  │  ├─ index.html
│  │  └─ js
│  │     ├─ api.js
│  │     ├─ app.js
│  │     ├─ chart.js
│  │     ├─ state.js
│  │     ├─ ui.js
│  │     └─ ws.js
│  ├─ tests
│  │  ├─ test_cs_encoder.py
│  │  ├─ test_quality.py
│  │  ├─ test_storage.py
│  │  └─ test_validator.py
│  ├─ tools
│  │  ├─ live_visualizer.py
│  │  ├─ test_single_signal.py
│  │  ├─ verify_phi.py
│  │  └─ __init__.py
│  ├─ __init__.py
│  └─ __main__.py
└─ skills-lock.json

```
```
IoTProject
├─ .agents
│  ├─ rules
│  │  └─ graphify.md
│  ├─ skills
│  │  ├─ cavecrew
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-commit
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-compress
│  │  │  ├─ README.md
│  │  │  ├─ scripts
│  │  │  │  ├─ benchmark.py
│  │  │  │  ├─ cli.py
│  │  │  │  ├─ compress.py
│  │  │  │  ├─ detect.py
│  │  │  │  ├─ validate.py
│  │  │  │  ├─ __init__.py
│  │  │  │  └─ __main__.py
│  │  │  ├─ SECURITY.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-help
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  ├─ caveman-review
│  │  │  ├─ README.md
│  │  │  └─ SKILL.md
│  │  └─ caveman-stats
│  │     ├─ README.md
│  │     └─ SKILL.md
│  └─ workflows
│     └─ graphify.md
├─ archive
│  ├─ firmware
│  │  ├─ CS_Sensor_gaussian_lasso.cpp
│  │  └─ CS_Sensor_gaussian_lasso.h
│  ├─ README.md
│  └─ server
│     ├─ config.py
│     ├─ cs_utils.py
│     └─ test_single_signal.py
├─ docs
│  ├─ breadboard_v01.png
│  ├─ breadboard_v02.png
│  ├─ mesh_architecture.md
│  ├─ prototype_v01.jpeg
│  ├─ prototype_v02_bottom.jpeg
│  ├─ prototype_v02_top.jpeg
│  ├─ schematic_v01.png
│  └─ schematic_v02.png
├─ firmware
│  ├─ include
│  │  ├─ config
│  │  │  ├─ features.h
│  │  │  ├─ hardware.h
│  │  │  ├─ mesh_settings.h
│  │  │  └─ tuning.h
│  │  ├─ Config.h
│  │  └─ utils
│  │     └─ Logger.h
│  ├─ lib
│  │  ├─ CS_Model_Gaussian
│  │  │  ├─ CS_Sensor.cpp
│  │  │  └─ CS_Sensor.h
│  │  ├─ CS_Model_Lasso
│  │  │  ├─ CS_Sensor.cpp
│  │  │  └─ CS_Sensor.h
│  │  ├─ EspNowMesh
│  │  │  ├─ EspNowMesh.cpp
│  │  │  ├─ EspNowMesh.h
│  │  │  ├─ MeshPackets.h
│  │  │  ├─ MeshRouting.cpp
│  │  │  └─ MeshRouting.h
│  │  ├─ HealthSensors
│  │  │  ├─ Sensor_MPU.cpp
│  │  │  ├─ Sensor_MPU.h
│  │  │  ├─ Sensor_PPG.cpp
│  │  │  └─ Sensor_PPG.h
│  │  ├─ Network_Mqtt
│  │  │  ├─ Network_Mqtt.cpp
│  │  │  └─ Network_Mqtt.h
│  │  ├─ Routing
│  │  │  ├─ DynamicRouter.cpp
│  │  │  └─ DynamicRouter.h
│  │  └─ Watchdog
│  │     ├─ Watchdog.cpp
│  │     └─ Watchdog.h
│  ├─ platformio.ini
│  ├─ src
│  │  ├─ main.cpp
│  │  ├─ task_cs_sender.cpp
│  │  └─ task_mesh_handler.cpp
│  └─ test
│     ├─ test_cs_encoder.cpp
│     └─ test_mesh_routing.cpp
├─ graphify-out
│  ├─ .graphify_labels.json
│  ├─ .graphify_python
│  ├─ .graphify_root
│  ├─ cost.json
│  ├─ graph.html
│  ├─ graph.json
│  ├─ GRAPH_REPORT.md
│  └─ manifest.json
├─ README.md
├─ server
│  ├─ .env.example
│  ├─ apps
│  │  ├─ dashboard
│  │  │  ├─ app.py
│  │  │  ├─ hub.py
│  │  │  ├─ routes
│  │  │  │  ├─ events.py
│  │  │  │  ├─ maintenance.py
│  │  │  │  ├─ metrics.py
│  │  │  │  ├─ nodes.py
│  │  │  │  ├─ status.py
│  │  │  │  ├─ windows.py
│  │  │  │  └─ __init__.py
│  │  │  ├─ websocket.py
│  │  │  ├─ __init__.py
│  │  │  └─ __main__.py
│  │  ├─ main_app.py
│  │  ├─ ml_inference
│  │  │  ├─ anomaly.py
│  │  │  ├─ inference.py
│  │  │  ├─ models
│  │  │  ├─ predictor.py
│  │  │  ├─ __init__.py
│  │  │  └─ __main__.py
│  │  ├─ reconstruct
│  │  │  ├─ listener.py
│  │  │  ├─ node_state.py
│  │  │  ├─ notifier.py
│  │  │  ├─ processor.py
│  │  │  ├─ __init__.py
│  │  │  └─ __main__.py
│  │  ├─ __init__.py
│  │  └─ __main__.py
│  ├─ core
│  │  ├─ config.py
│  │  ├─ logger.py
│  │  ├─ quality.py
│  │  ├─ storage.py
│  │  ├─ validator.py
│  │  └─ __init__.py
│  ├─ cs
│  │  ├─ gaussian.py
│  │  ├─ lasso.py
│  │  ├─ matrices
│  │  ├─ router.py
│  │  └─ __init__.py
│  ├─ data
│  │  └─ backups
│  ├─ graphify-out
│  │  ├─ .graphify_labels.json
│  │  ├─ .graphify_root
│  │  ├─ graph.html
│  │  ├─ graph.json
│  │  ├─ GRAPH_REPORT.md
│  │  └─ manifest.json
│  ├─ pyproject.toml
│  ├─ README.md
│  ├─ requirements-dev.txt
│  ├─ requirements.txt
│  ├─ static
│  │  ├─ css
│  │  │  └─ style.css
│  │  ├─ index.html
│  │  └─ js
│  │     ├─ api.js
│  │     ├─ app.js
│  │     ├─ chart.js
│  │     ├─ state.js
│  │     ├─ ui.js
│  │     └─ ws.js
│  ├─ tests
│  │  ├─ test_cs_gaussian.py
│  │  ├─ test_full_pipeline.py
│  │  ├─ test_quality.py
│  │  ├─ test_storage.py
│  │  └─ test_validator.py
│  ├─ tools
│  │  ├─ live_visualizer.py
│  │  ├─ test_single_signal.py
│  │  ├─ verify_phi.py
│  │  └─ __init__.py
│  ├─ __init__.py
│  └─ __main__.py
└─ skills-lock.json

```