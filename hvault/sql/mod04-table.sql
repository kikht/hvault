CREATE FOREIGN TABLE mod04 (
    file_id   int4        OPTIONS (type 'file_index'),
    line_id   int4        OPTIONS (type 'line_index'),
    sample_id int4        OPTIONS (type 'sample_index'),
    point     geometry    OPTIONS (type 'point'),
    footprint geometry    OPTIONS (type 'footprint'),
    time      timestamp   OPTIONS (type 'time'),

    Aerosol_Type_Land                             int2    OPTIONS (sds 'Aerosol_Type_Land', type 'int2'),
    Angstrom_Exponent_Land                        float8  OPTIONS (sds 'Angstrom_Exponent_Land'),
    Cloud_Fraction_Land                           float8  OPTIONS (sds 'Cloud_Fraction_Land'),
    Cloud_Fraction_Ocean                          float8  OPTIONS (sds 'Cloud_Fraction_Ocean'),
    Cloud_Mask_QA                                 char    OPTIONS (sds 'Cloud_Mask_QA', type 'byte'),
    Corrected_Optical_Depth_Land_wav2p1           float8  OPTIONS (sds 'Corrected_Optical_Depth_Land_wav2p1'),
    Deep_Blue_Aerosol_Optical_Depth_550_Land      float8  OPTIONS (sds 'Deep_Blue_Aerosol_Optical_Depth_550_Land'),
    Deep_Blue_Aerosol_Optical_Depth_550_Land_STD  float8  OPTIONS (sds 'Deep_Blue_Aerosol_Optical_Depth_550_Land_STD'),
    Deep_Blue_Angstrom_Exponent_Land              float8  OPTIONS (sds 'Deep_Blue_Angstrom_Exponent_Land'),
    Fitting_Error_Land                            float8  OPTIONS (sds 'Fitting_Error_Land'),
    Image_Optical_Depth_Land_And_Ocean            float8  OPTIONS (sds 'Image_Optical_Depth_Land_And_Ocean'),
    Mass_Concentration_Land                       float8  OPTIONS (sds 'Mass_Concentration_Land'),
    Number_Pixels_Used_Ocean                      int2    OPTIONS (sds 'Number_Pixels_Used_Ocean', type 'int2'),
    Optical_Depth_Land_And_Ocean                  float8  OPTIONS (sds 'Optical_Depth_Land_And_Ocean'),
    Optical_Depth_Ratio_Small_Land                float8  OPTIONS (sds 'Optical_Depth_Ratio_Small_Land'),
    Optical_Depth_Ratio_Small_Land_And_Ocean      float8  OPTIONS (sds 'Optical_Depth_Ratio_Small_Land_And_Ocean'),
    Scan_Start_Time                               float8  OPTIONS (sds 'Scan_Start_Time'),
    Scattering_Angle                              float8  OPTIONS (sds 'Scattering_Angle'),
    Sensor_Azimuth                                float8  OPTIONS (sds 'Sensor_Azimuth'),
    Sensor_Zenith                                 float8  OPTIONS (sds 'Sensor_Zenith'),
    Solar_Azimuth                                 float8  OPTIONS (sds 'Solar_Azimuth'),
    Solar_Zenith                                  float8  OPTIONS (sds 'Solar_Zenith')
) SERVER hvault_service
  OPTIONS (catalog 'mod04_catalog',
           shift_longitude 'true');
