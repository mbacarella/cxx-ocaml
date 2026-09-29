type t = ..
module type S = sig type t += E end
module M1 : S = struct type t += E end
let f ?opt:((module M) = (module M1 : S)) () = fun M.E -> ()
[@@ocaml.warning "-8"]
