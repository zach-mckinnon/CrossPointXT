Import("env")

# The standalone S3 writer uses the shared serialized storage API, without
# pulling the Xteink C3 display, GPIO, or power drivers into its build.
if env["PIOENV"] == "heltec_e213_writer":
    env.Replace(SRC_FILTER=["+<HalStorage.cpp>"])
