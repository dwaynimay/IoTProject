import sys
import platform

# Workaround for Python 3.13 WMI hang on Windows when importing numpy/scipy
# This avoids slow/hanging startup when calling platform.uname() or platform.machine()
if sys.platform == "win32" and hasattr(platform, '_wmi_query'):
    _original_wmi_query = platform._wmi_query
    
    def _fast_wmi_query(*args, **kwargs):
        # WMI queries can hang for up to 30 seconds on some Windows machines.
        # We mock the response of the OS version query to avoid the hang.
        return ('10.0.19041', '1', 'Primary', '0', '0')
        
    platform._wmi_query = _fast_wmi_query
