module X = struct module type S = sig type t end end
let pack (type a) (x : a) = (module struct type t = a end : X.S with type t = a)
