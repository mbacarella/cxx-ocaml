module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module F (X : sig end) = struct
 module B : S with type t = (module A.A_S) = A
end
