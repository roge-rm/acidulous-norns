// Runs the Acidulous engine between the norns inputs and outputs.
// Command: start. The script then talks to the plugin over OSC on port 57150
// (see host/src/bridge.h). Patches are read from the script's data/banks.
Engine_Acidulous : CroneEngine {
    var synth;

    *new { arg context, doneCallback;
        ^super.new(context, doneCallback);
    }

    alloc {
        SynthDef(\acidulous, { arg in_l, in_r, out;
            Out.ar(out, Acidulous.ar(In.ar(in_l), In.ar(in_r)));
        }).add;
        // The def has to reach scsynth before start makes the synth.
        context.server.sync;

        this.addCommand("start", "", { arg msg;
            this.startEngine;
        });
    }

    startEngine {
        var banks = Platform.userHomeDir ++ "/dust/code/acidulous/data/banks";
        synth !? { synth.free; synth = nil };
        context.server.sendMsg(\cmd, "AcidulousStart", banks);
        synth = Synth(\acidulous, [
            \in_l, context.in_b[0].index,
            \in_r, context.in_b[1].index,
            \out, context.out_b.index
        ], context.xg);
    }

    free {
        synth !? { synth.free };
    }
}
