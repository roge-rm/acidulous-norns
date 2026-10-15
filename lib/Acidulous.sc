// The Acidulous engine as a UGen. It runs once /cmd AcidulousStart has been
// sent (Engine_Acidulous does this). Inputs: left and right audio in, which
// machines that listen to the input hear. Outputs: the engine's master, L and R.

Acidulous : MultiOutUGen {
    *ar { arg inL, inR;
        ^this.multiNew('audio', inL, inR)
    }
    init { arg ... theInputs;
        inputs = theInputs;
        ^this.initOutputs(2, rate)
    }
}
