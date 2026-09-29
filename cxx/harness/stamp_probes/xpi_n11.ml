module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module F (X : sig end) = struct include A end
