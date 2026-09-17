module type S = sig type t end
module M = struct type 'a arg_t = 'at constraint 'a = (module S with type t =
  'at) end
