module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
let f () : int G(M1).t = [];;
