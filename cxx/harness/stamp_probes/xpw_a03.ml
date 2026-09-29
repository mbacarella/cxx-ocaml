module type S = sig type t type u end
type t = (module S with type t = unit)
