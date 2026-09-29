module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
module N = G(M1);;
let f (x : int G(M1).t) = x;;
