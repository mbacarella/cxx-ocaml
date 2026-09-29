module type ORD = sig type t end
module type SET = sig type t end
let f () = let module B (F : functor (X : ORD) -> SET) = struct end in ()
