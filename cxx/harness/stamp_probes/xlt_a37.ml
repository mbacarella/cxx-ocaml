module Id (X : sig type t end) = X
module A = struct type t end
class c = object method m : Id (A).t option = None end
