module type S = sig type t end
type ('a, 'b) arg_t = 'at constraint 'a = (module S with type t = 'at)
  constraint 'b = (module S with type t = 'at)
