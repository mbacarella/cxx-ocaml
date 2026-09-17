module type S = sig type t module type T = sig type s end end
type u = (module S with type t = int)
