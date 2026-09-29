module type S = sig type t end
type 'a arg_t = 'at constraint (module S with type t = 'at) = 'a
