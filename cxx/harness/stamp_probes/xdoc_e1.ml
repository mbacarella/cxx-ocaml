(* Empty `struct … end` / `sig … end` bodies holding only floating doc comments:
   parser.mly's extra_text gives `text post @ text post_extras` at the body's end
   (odoc's test/xref2/labels: the port emitted the last first and lost the first) *)
module M0 = struct
  (** A *)
end
module type S0 = sig
  (** A *)
end
module M1 = struct
  (** A *)
  (** B *)
end
module type S1 = sig
  (** A *)
  (** B *)
end
module M2 = struct
  (** A *)

  (** B *)
end
module type S2 = sig
  (** A *)

  (** B *)
end
module M3 = struct
  (** A *)

  (** B *)

  (** C *)
end
module type S3 = sig
  (** A *)

  (** B *)

  (** C *)
end
module M4 = struct
  

  (** A *)

  (** B *)
end
module type S4 = sig
  

  (** A *)

  (** B *)
end
module M5 = struct
  (** A *)

  (** B *)


end
module type S5 = sig
  (** A *)

  (** B *)


end
module M6 = struct
  (** *)

  (** B *)

  (** C *)
end
module type S6 = sig
  (** *)

  (** B *)

  (** C *)
end
