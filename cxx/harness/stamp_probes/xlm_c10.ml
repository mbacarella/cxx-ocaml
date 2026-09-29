module F (X : sig type t end) = struct
  type 'a t = X.t list let empty : 'a t = [] end
module A = struct type t = int end
let f () =
   let module N = F(A) in
   ()
