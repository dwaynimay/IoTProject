```
IoTProject
├─ .gitignore
├─ README.md
├─ archive
│  ├─ README.md
│  ├─ firmware
│  │  ├─ CS_Sensor_gaussian_lasso.cpp
│  │  └─ CS_Sensor_gaussian_lasso.h
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
│  ├─ .gitignore
│  ├─ include
│  │  ├─ Config.h
│  │  ├─ config
│  │  │  ├─ credentials.h
│  │  │  ├─ credentials.h.example
│  │  │  ├─ features.h
│  │  │  ├─ hardware.h
│  │  │  ├─ mesh_settings.h
│  │  │  └─ tuning.h
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
├─ new_feature
├─ server
│  ├─ .env
│  ├─ .env.example
│  ├─ README.md
│  ├─ __init__.py
│  ├─ __main__.py
│  ├─ apps
│  │  ├─ __init__.py
│  │  ├─ __main__.py
│  │  ├─ dashboard
│  │  │  ├─ __init__.py
│  │  │  ├─ __main__.py
│  │  │  ├─ app.py
│  │  │  ├─ hub.py
│  │  │  ├─ routes
│  │  │  │  ├─ __init__.py
│  │  │  │  ├─ events.py
│  │  │  │  ├─ maintenance.py
│  │  │  │  ├─ metrics.py
│  │  │  │  ├─ nodes.py
│  │  │  │  ├─ status.py
│  │  │  │  └─ windows.py
│  │  │  └─ websocket.py
│  │  ├─ main_app.py
│  │  ├─ ml_inference
│  │  │  ├─ __init__.py
│  │  │  ├─ __main__.py
│  │  │  ├─ anomaly.py
│  │  │  ├─ inference.py
│  │  │  ├─ models
│  │  │  │  └─ .gitkeep
│  │  │  └─ predictor.py
│  │  └─ reconstruct
│  │     ├─ __init__.py
│  │     ├─ __main__.py
│  │     ├─ listener.py
│  │     ├─ node_state.py
│  │     ├─ notifier.py
│  │     └─ processor.py
│  ├─ core
│  │  ├─ __init__.py
│  │  ├─ config.py
│  │  ├─ logger.py
│  │  ├─ quality.py
│  │  ├─ storage.py
│  │  └─ validator.py
│  ├─ cs
│  │  ├─ __init__.py
│  │  ├─ gaussian.py
│  │  ├─ lasso.py
│  │  ├─ matrices
│  │  │  └─ .gitkeep
│  │  └─ router.py
│  ├─ data
│  │  ├─ .gitignore
│  │  └─ backups
│  │     └─ .gitkeep
│  ├─ logs
│  │  └─ .gitignore
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
│  │  ├─ test_cs_gaussian.py
│  │  ├─ test_full_pipeline.py
│  │  ├─ test_quality.py
│  │  ├─ test_storage.py
│  │  └─ test_validator.py
│  └─ tools
│     ├─ __init__.py
│     ├─ live_visualizer.py
│     ├─ print_tree.py
│     ├─ test_single_signal.py
│     └─ verify_phi.py
└─ skills-lock.json
```
