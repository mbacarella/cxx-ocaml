type t = ..
module type S = sig type t += E end
module M1 : S = struct type t += E end
module M2 : S = struct type t += E end
let binding1 (module M : S) ?(opt = M.E) () = opt
let binding2 ?opt:((module M) = (module M1 : S)) M.E = ()
[@@ocaml.warning "-8"];;
let () = binding2 ~opt:(module M2) M2.E;;
