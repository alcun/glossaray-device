"""Inject an optional host-local device token into a trusted USB build.

The value comes only from the GLOSSARAY_DEVICE_TOKEN process environment. It is
never written to the repository or printed by this script.
"""

import os

Import("env")

token = os.environ.get("GLOSSARAY_DEVICE_TOKEN", "")
if token:
    env.Append(
        CPPDEFINES=[
            ("GLOSSARAY_PROVISIONED_DEVICE_TOKEN", env.StringifyMacro(token)),
        ]
    )

api_url = os.environ.get("GLOSSARAY_API_URL", "")
if api_url:
    env.Append(
        CPPDEFINES=[
            ("GLOSSARAY_PROVISIONED_API_URL", env.StringifyMacro(api_url.rstrip("/"))),
        ]
    )

provisioning_revision = os.environ.get("GLOSSARAY_PROVISIONING_REVISION", "")
if provisioning_revision:
    env.Append(
        CPPDEFINES=[
            (
                "GLOSSARAY_PROVISIONING_REVISION",
                env.StringifyMacro(provisioning_revision),
            ),
        ]
    )
