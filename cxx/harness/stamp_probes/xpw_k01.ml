module type S = sig type t end
type 'a arg_t = 'at constraint 'a = (module S with type t = 'at)
type t = (module S with type t = unit)
type u = t arg_t
