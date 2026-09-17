module type S = sig type t end
type 'a arg_t = 'at constraint 'a = (module S with type t = 'at) constraint
  'at = (module S)
