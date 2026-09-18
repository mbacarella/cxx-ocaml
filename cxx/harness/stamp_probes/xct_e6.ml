module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
type u = int G(M1).t;;
