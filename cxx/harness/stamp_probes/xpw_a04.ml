module X = struct module type S = sig type t end end
type 'a arg_t = 'at constraint 'a = (module X.S with type t = 'at)
