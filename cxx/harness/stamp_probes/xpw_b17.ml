module type S = sig type t end
type t = A of (module S with type t = unit)
