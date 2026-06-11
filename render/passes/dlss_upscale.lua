return RenderGraphPass {
    type     = "DLSSUpscale",
    menuPath = "Upscaling/DLSS Upscale",
    inputs   = { "color", "depth", "motion" },
    outputs  = { "color" },
    params   = {
        { name = "enabled", type = "bool", default = true },
    },

    setup = function(ctx)
        local color = ctx:getInput("color")
        local out = nil
        if ctx:paramBool("enabled", true) then
            out = ctx:createUpscalerOutput {
                name     = "DLSS Upscale Output",
                color    = color,
                depth    = ctx:getInput("depth"),
                motion   = ctx:getInput("motion"),
            }
        else
            out = color
        end
        ctx:setOutput("color", out)
        ctx:setResource("FinalCompositionSource", out)
    end,

    execute = function(rc)
        rc:evaluateUpscaler()
    end,
}
