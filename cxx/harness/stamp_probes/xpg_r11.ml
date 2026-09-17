module type S = sig type t end
module M1 = struct type t = int end
let f ?m:(x = (module M1 : S)) () = ()
