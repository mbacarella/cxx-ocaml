module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
exception E of int G(M1).t;;
