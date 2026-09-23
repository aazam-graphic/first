#!/usr/bin/env python3
# Reconfigure build with voice_ai stub components after CMakeLists changes
import os, sys, subprocess

os.environ['IDF_PATH'] = 'C:\\esp\\v6.0.2\\esp-idf'
os.environ['IDF_TOOLS_PATH'] = 'C:\\Espressif'
os.environ['IDF_PYTHON_ENV_PATH'] = 'C:\\Espressif\\tools\\python\\v6.0.2\\venv'

project = 'C:\\Users\\aazam\\Desktop\\xbox360_controller'
os.chdir(project)

# clean + build
r = subprocess.run(['idf.py', 'build'], capture_output=True, text=True)
print("STDOUT:", r.stdout[-3000:])
print("STDERR:", r.stderr[-1000:])
print("EXIT:", r.returncode)
sys.exit(r.returncode)
