module F (X : sig type t end) = struct
  type 'a t let empty : 'a t = Obj.magic 0 end
module A = struct type t = int end
let f () =
   let module N = F(A) in
   ()
