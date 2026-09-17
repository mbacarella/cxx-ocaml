module type S = sig type t end
module M1 = struct type t = int end
let f ?(m = (module M1 : S)) () = ()
let () = f ()
