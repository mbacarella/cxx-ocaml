module type E = sig end
module I : E = Int let _ = (module I : E)
