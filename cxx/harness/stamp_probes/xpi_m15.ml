module A = struct type t = int end
module type S = sig type t end
module F (X : sig end) = struct
 let f (type a) (module X : S with type t = a) = ()
 let _ = f (module A)
end
