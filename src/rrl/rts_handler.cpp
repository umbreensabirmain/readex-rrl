#include <nitro/dl/dl.hpp>
#include <rrl/rts_handler.hpp>
#include <util/environment.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <json.hpp>

using json = nlohmann::json;

namespace rrl
{
/**
 * Constructor
 *
 * @brief Constructor
 *
 * Initializes the rts handler.
 * It gets the reference to the instance of tuning model manager.
 *
 * @param tmm shared pointer to a tuning model manager instance
 *
 **/
rts_handler::rts_handler(
    std::shared_ptr<tmm::tuning_model_manager> tmm, std::shared_ptr<cal::calibration> cal)
    : is_inside_root(false),
      tmm_(tmm),
      cal_(cal),
      pc_(parameter_controller::instance()),
      region_status_(tmm::insignificant)
{
    logging::debug("RTS") << " initializing";

    try
    {
        significant_duration = std::chrono::milliseconds(
            std::stoi(rrl::environment::get("SIGNIFICANT_DURATION_MS", "100")));
    }
    catch (std::invalid_argument &e)
    {
        logging::fatal("RTS")
            << "Invalid argument given for SCOREP_RRL_SIGNIFICANT_DURATION_MS. Value "
               "hase to be a positiv int.";
        throw;
    }
    catch (std::out_of_range &e)
    {
        logging::fatal("RTS") << "Value of SCOREP_RRL_SIGNIFICANT_DURATION out of range.";
        throw;
    }

    auto input_id_file = rrl::environment::get("INPUT_IDENTIFIER_SPEC_FILE", "");
    if (!input_id_file.empty())
    {
        std::ifstream input_id_file_(input_id_file);
        if (!input_id_file_.is_open())
        {
            if (input_id_file_.fail())
            {
                logging::error("RTS") << "Could not open Input Identifer File: " << strerror(errno);
            }
            else
            {
                logging::error("RTS") << "Could not open Input Identifer File";
            }
        }
        json input_id;
        input_id_file_ >> input_id;

        for (json::iterator it = input_id.begin(); it != input_id.end(); ++it)
        {
            if (it.key() == "collect_num_threads")
            {
                if (it.value().is_boolean() && it.value().get<bool>())
                {
                    try
                    {
                        auto omp_get_max_threads =
                            nitro::dl::dl(nitro::dl::self).load<int(void)>("omp_get_max_threads");
                        auto num_threads = omp_get_max_threads();
                        input_identifiers_["threads"] = std::to_string(num_threads);
                        logging::debug("RTS") << "Added Input Identifier \"threads\" with value \""
                                              << num_threads << "\"";
                    }
                    catch (nitro::dl::exception &e)
                    {
                        logging::debug("RTS")
                            << "could not find \"omp_get_max_threads\". Reason:" << e.what();
                    }
                }
                continue;
            }
            if (it.key() == "collect_num_processes")
            {
                if (it.value().is_boolean() && it.value().get<bool>())
                {
                    int size = scorep::call::ipc_get_size();
                    input_identifiers_["processes"] = std::to_string(size);
                    logging::debug("RTS")
                        << "Added Input Identifier \"processes\" with value \"" << size << "\"";
                }
                continue;
            }
            if (it.value().is_string())
            {
                input_identifiers_[it.key()] = it.value().get<std::string>();
                logging::debug("RTS")
                    << "Added Input Identifier \"" << it.key() << "\" with value \""
                    << it.value().get<std::string>() << "\"";
            }
            else
            {
                logging::warn("RTS")
                    << "Canno't parse key \"" << it.key() << "\". Value is not a string";
            }
        }
    }
}

/**
 * Destructor
 *
 * @brief Destructor
 * Deletes the rts handler
 *
 **/
rts_handler::~rts_handler()
{
    logging::debug("RTS") << " finalizing";
}

/** Checks if all information for loading the config are present, and loads the
 * current configuration.
 *
 * This function has three main purposes:
 * * check if all expected additional identifiers are collected
 * * check if the region duration is sufficient long
 * * check if callibration is needed.
 * The check have to occure in this order. Just when all additional identifiers are collected, we
 * can request the excecution time. The execution time again indicate if a region needs
 * recallibration. However this is currently not implemented.
 *
 * TODO: Rename the function as the name is not right anymore.
 *
 */
void rts_handler::load_config()
{
    logging::trace("RTS") << "load_config";
    /* handle known significant regions
     */
    if (region_status_ == tmm::significant)
    {
        if (tmm_->get_region_identifiers(callpath_.back().region_id) ==
            callpath_.back().id_set.size())
        {
            auto configs = tmm_->get_current_rts_configuration(callpath_, input_identifiers_);
            if (configs.size() > 0)
            {
                if (tmm_->get_exectime(callpath_) >= significant_duration)
                {
                    pc_.set_parameters(configs);
                    /* In theory each region has one enter and one exit. If a region has additional
                     * identifiers, it is supposed to set the config when the last additional
                     * identifier is loaded. However it is possible, that a region has different
                     * additional identifers that load different configs. To be sure that this cases
                     * does not occure, we do the assertion here. If we change this assumption we
                     * have to change the implemetation here and for the exit regions.
                     */
                    assert(callpath_.back().config_set == false);
                    callpath_.back().config_set = true;
                }
            }
            else
            {
                if (tmm_->get_calibration_type() != tmm::none)
                {
                    logging::trace("RTS")
                        << "didn't get a configuration but all additional identifiers "
                           "are collected. Starting calibration.";
                    callpath_.back().calibrate = true;
                }
            }
        }
        else if (tmm_->get_region_identifiers(callpath_.back().region_id) >
                 callpath_.back().id_set.size())
        {
            logging::warn("RTS") << "got more additional ID's than expected. Got: "
                                 << callpath_.back().id_set.size() << " expected :"
                                 << tmm_->get_region_identifiers(callpath_.back().region_id)
                                 << "\nWe currently do not deal with this situation. However, you "
                                    "can rerun DTA to make the TM aware of the new additional ID's";
        }
    }

    /** Calibrate when the region is unkown.
     *
     * How to work with additional identifiers (e.g. SCOREP_USER_PARAMETERS) that are in unknown
     * regions?
     */
    if ((tmm_->get_calibration_type() != tmm::none) && (region_status_ == tmm::unknown))
    {
        callpath_.back().calibrate = true;
    }

    /** We can be sure, that at this point no configuration is set by the TMM. So we can set the
     * config requested from the CAL.
     */
    if (callpath_.back().calibrate)
    {
        auto configs = cal_->calibrate_region(callpath_.back().region_id);
        if (configs.size() > 0)
        {
            pc_.set_parameters(configs);
            /* In theory each region has one enter and one exit. If a region has additional
             * identifiers, it is supposed to set the config when the last additional identifier
             * is loaded. However it is possible, that a region has different additional
             * identifers that load different configs. To be sure that this cases does not
             * occure, we do the assertion here. If we change this assumption we have to change
             * the implemetation here and for the exit regions.
             */
            assert(callpath_.back().config_set == false);
            callpath_.back().config_set = true;
        }
    }
}

/**This function handles the enter regions.
 *
 * This function handles the enter regions.
 * It gets the region id and maintains the callpath.
 *
 * It passes region id to tuning model manger to detect if a region is a
 * significant rts.
 *
 * After detecting the rts, configurations for the respective rts is requested
 * from tuning model manger. This configuration is passed to parameter
 * controller for setting the parameters.
 *
 * @param region_id Score-P region identifier
 * @param locationData can be used with scorep::location_get_id() and
 *        scorep::location_get_gloabl_id() to obtain the location of the call.
 **/
void rts_handler::enter_region(uint32_t region_id, SCOREP_Location *locationData)
{
    auto elem = tmm::simple_callpath_element(region_id, tmm::identifier_set());
    if (!is_inside_root)
    {
        if (tmm_->is_root(elem))
        {
            is_inside_root = true;
            logging::trace("RTS") << "tmm_->is_root(elem) = true";
        }
        else
        {
            logging::trace("RTS") << "tmm_->is_root(elem) = false";
            return;
        }
    }
    logging::trace("RTS") << "is_inside_root = true";

    callpath_.push_back(elem);

    region_status_ = tmm_->is_significant(region_id);

    load_config();
}

/**This function handles the exit regions.
 *
 * This function handles the exit regions.
 * It gets the region id and maintains the callpath.
 *
 * @param region_id Score-P region identifier
 * @param locationData can be used with scorep::location_get_id() and
 *        scorep::location_get_gloabl_id() to obtain the location of the call.
 **/
void rts_handler::exit_region(uint32_t region_id, SCOREP_Location *locationData)
{
    if (!is_inside_root)
    {
        return;
    }

    auto elem = tmm::simple_callpath_element(region_id, tmm::identifier_set());
    if ((callpath_.size() == 1) && (tmm_->is_root(elem) == true))
    {
        is_inside_root = false;
    }

    auto callpath_elem = callpath_.back();
    if (callpath_elem.region_id != elem.region_id)
    {
        logging::error("RTS") << "callpath not intact. RegionId removed from "
                              << "the callpath: " << callpath_elem.region_id << " RegionId "
                              << "passed by Score-P: " << region_id;
        logging::error("RTS") << "full callpath_elem on stack:\n" << callpath_elem;
        logging::error("RTS") << "full elem from region:\n" << elem;
        logging::error("RTS") << "won't change the callpath.";
    }
    if (callpath_elem.calibrate)
    {
        auto config = cal_->request_configuration(callpath_elem.region_id);
        tmm_->store_configuration(callpath_, config, std::chrono::milliseconds::max());
    }
    else
    {
        if (callpath_.back().config_set)
        {
            pc_.unset_parameters();
        }
    }
    callpath_.pop_back();
}

void rts_handler::create_location(SCOREP_LocationType location_type, std::uint32_t location_id)
{
    pc_.create_location(location_type, location_id);
}
void rts_handler::delete_location(SCOREP_LocationType location_type, std::uint32_t location_id)
{
    pc_.delete_location(location_type, location_id);
}

/** Handling user parameters
 *
 * @param user_parameter_name name of the user parameter
 * @param value value of the parameter
 * @param locationData can be used with scorep::location_get_id() and
 *        scorep::location_get_gloabl_id() to obtain the location of the call.
 *
 *
 */
void rts_handler::user_parameter(
    std::string user_parameter_name, uint64_t value, SCOREP_Location *locationData)
{
    if (!is_inside_root)
    {
        logging::warn("RTS") << "Will ignore the user parameter \"" << user_parameter_name
                             << "\" as it is not defined in the given root phase (OA Phase)";
        return;
    }

    logging::trace("RTS") << " Got additional user param uint \"" << user_parameter_name
                          << "(hash: " << user_parameter_hash_(user_parameter_name)
                          << "\": " << value;
    callpath_.back().id_set.add_identifier(user_parameter_name, value);

    load_config();
}

/** Handling user parameters
 *
 * @param user_parameter_name name of the user parameter
 * @param value value of the parameter
 *
 */
void rts_handler::user_parameter(
    std::string user_parameter_name, int64_t value, SCOREP_Location *locationData)
{
    if (!is_inside_root)
    {
        logging::warn("RTS") << "Will ignore the user parameter \"" << user_parameter_name
                             << "\" as it is not defined in the given root phase (OA Phase)";
        return;
    }

    logging::trace("RTS") << " Got additional user param int \"" << user_parameter_name
                          << "(hash: " << user_parameter_hash_(user_parameter_name)
                          << "\": " << value;
    callpath_.back().id_set.add_identifier(user_parameter_name, value);

    load_config();
}

/** Handling user parameters
 *
 * @param user_parameter_name name of the user parameter
 * @param value value of the parameter
 * @param locationData can be used with scorep::location_get_id() and
 *        scorep::location_get_gloabl_id() to obtain the location of the call.
 *
 */
void rts_handler::user_parameter(
    std::string user_parameter_name, std::string value, SCOREP_Location *locationData)
{
    if (!is_inside_root)
    {
        logging::warn("RTS") << "Will ignore the user parameter \"" << user_parameter_name
                             << "\" as it is not defined in the given root phase (OA Phase)";
        return;
    }

    logging::trace("RTS") << " Got additional user param string \"" << user_parameter_name
                          << "(hash: " << user_parameter_hash_(user_parameter_name)
                          << "\": " << value;
    callpath_.back().id_set.add_identifier(user_parameter_name, value);

    load_config();
}
}
