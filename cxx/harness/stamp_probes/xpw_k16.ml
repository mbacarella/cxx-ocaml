module type S = sig type t end
module type T = sig val x : (module S with type t = unit) end
