module type E = sig end
module I = Int let _ = (module I : E)
