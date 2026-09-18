module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
class type u = F(M1).t and v = F(M1).t;;
