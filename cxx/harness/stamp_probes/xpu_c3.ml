module type E = sig end
module F (X : sig end) = struct let _ = (module Int : E) end
