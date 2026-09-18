module G (X : sig end) = struct type 'a t = 'a list end;;
module M1 = struct end;;
class c = object method m (x : int G(M1).t) = x end;;
