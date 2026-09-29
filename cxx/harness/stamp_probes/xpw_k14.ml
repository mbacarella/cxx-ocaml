module type S = sig type t end
module M : sig type 'a arg_t = 'at constraint 'a = (module S with type t =
  'at) end = struct type 'a arg_t = 'at constraint 'a = (module S with type t
  = 'at) end
