module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module B = (A : S with type t = (module A.A_S))
