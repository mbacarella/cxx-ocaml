module Id (X : sig type t end) = X
module A = struct type t end
class type ct = object method m : Id (A).t end
