module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module X : sig module N : S with type t = (module A.A_S) end =
 struct module N = A end
