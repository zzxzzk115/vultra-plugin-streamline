local status = Upscaler.status()
print(string.format(
    "[Streamline] active=%s available=%s enabled=%s mode=%s message=%s",
    tostring(status.active),
    tostring(status.available),
    tostring(status.enabled),
    tostring(status.mode),
    tostring(status.message)
))

return {}
