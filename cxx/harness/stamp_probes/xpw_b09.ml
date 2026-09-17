module type S = sig type t end
module type S2 = S
type t = (module S2 with type t = unit)
