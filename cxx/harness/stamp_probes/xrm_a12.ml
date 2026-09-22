module rec A : sig type t = B.t type s = C.t end
  = struct type t = B.t type s = C.t end
and B : sig type t = C.t type u = t end
  = struct type t = C.t type u = t end
and C : sig type t = int type v = X | Y end
  = struct type t = int type v = X | Y end
