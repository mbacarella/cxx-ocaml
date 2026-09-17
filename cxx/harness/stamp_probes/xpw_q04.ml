module X = struct module Y = struct module type S = sig type t end end end
module Y = X.Y
type 'a arg_t = 'at constraint 'a = (module Y.S with type t = 'at)
type t = (module X.Y.S with type t = unit)
let f (x : t arg_t) = ()
