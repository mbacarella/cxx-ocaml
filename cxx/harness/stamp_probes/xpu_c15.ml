module type E = sig end
module type T = sig type t val zero : t end let _ = (module Int : T)
