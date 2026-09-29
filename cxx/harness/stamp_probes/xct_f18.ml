module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
let f x = (x : int G(M1).t);;
