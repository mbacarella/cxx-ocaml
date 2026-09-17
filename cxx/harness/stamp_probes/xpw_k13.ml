module type S = sig type t end
module type T = sig type 'a arg_t = 'at constraint 'a = (module S with type t
  = 'at) end
