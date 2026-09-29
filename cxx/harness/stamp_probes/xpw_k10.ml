module type S = sig type t end
type 'a arg_t = 'at constraint 'a = (module S with type t = 'at)
type t = (module S with type t = unit)
let f (x : t arg_t) = ()
type u = t arg_t
