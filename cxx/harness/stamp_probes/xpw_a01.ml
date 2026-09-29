module type S = sig type t end
type t = (module S with type t = unit)
