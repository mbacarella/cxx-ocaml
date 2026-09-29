module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
let f () = let y : int G(M1).t = [] in ignore y;;
