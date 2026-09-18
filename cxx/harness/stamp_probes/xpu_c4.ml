module type E = sig end
module M = struct let _ = (module Int : E) end
